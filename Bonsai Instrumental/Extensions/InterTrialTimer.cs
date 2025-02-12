using System;
using System.ComponentModel;
using System.Reactive.Linq;
using Bonsai;

[Combinator]
[Description("Generates a timer with an optional random interval between the specified minimum and maximum durations.")]
[WorkflowElementCategory(ElementCategory.Source)]
public class InterTrialTimer
{
    // Properties for the timer configuration
    [Description("The relative time at which to produce the first value. If this value is less than or equal to zero, the timer will fire as soon as possible.")]
    public TimeSpan DueTime { get; set; }

    [Description("The period to produce subsequent values. If this value is equal to zero, the timer will fire only once.")]
    public TimeSpan Period { get; set; }

    [Description("The minimum duration for a random interval. If not set, the DueTime property will be used.")]
    public TimeSpan? MinDueTime { get; set; }

    [Description("The maximum duration for a random interval. If not set, the DueTime property will be used.")]
    public TimeSpan? MaxDueTime { get; set; }

    // Constructor to set default values
    public InterTrialTimer()
    {
        DueTime = TimeSpan.Zero;
        Period = TimeSpan.Zero;
    }

    // Process method to define the behavior of the timer
    public IObservable<long> Process()
    {
        return Observable.Defer(() =>
        {
            // Calculate the effective DueTime
            TimeSpan effectiveDueTime;
            if (MinDueTime.HasValue && MaxDueTime.HasValue)
            {
                var random = new Random();
                var minMs = MinDueTime.Value.TotalMilliseconds;
                var maxMs = MaxDueTime.Value.TotalMilliseconds;
                var randomMs = random.Next((int)minMs, (int)maxMs + 1);
                effectiveDueTime = TimeSpan.FromMilliseconds(randomMs);
            }
            else
            {
                effectiveDueTime = DueTime;
            }

            // Generate the timer sequence
            if (Period > TimeSpan.Zero)
            {
                return Observable.Timer(effectiveDueTime, Period);
            }
            else
            {
                return Observable.Timer(effectiveDueTime);
            }
        });
    }
}